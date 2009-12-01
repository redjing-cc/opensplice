namespace test.sacs
{
    /// <date>Jun 2, 2005</date>
    public class Listener1 : Test.Framework.TestCase
    {
        public Listener1()
            : base("sacs_listener_tc1", "sacs_listener", "listener", "Test if a DataReaderListener works."
                , "Test if a DataReaderListener works.", null)
        {
        }

        public override Test.Framework.TestResult Run()
        {
            DDS.DomainParticipantFactory factory;
            DDS.IDomainParticipant participant;
            DDS.DomainParticipantQos pqosHolder;
            DDS.TopicQos topQosHolder;
            DDS.ITopic topic;
            mod.tstTypeSupport typeSupport = null;
            mod.tstDataReader datareader;
            test.sacs.MyDataReaderListener listener;
            DDS.ISubscriber subscriber;
            DDS.SubscriberQos sqosHolder;
            DDS.DataReaderQos dqosHolder;
            DDS.IPublisher publisher;
            DDS.PublisherQos pubQosHolder;
            mod.tstDataWriter datawriter;
            DDS.DataWriterQos wqosHolder;
            Test.Framework.TestResult result;
            DDS.ReturnCode rc;
            string expResult = "DataReaderListener test succeeded.";
            result = new Test.Framework.TestResult(expResult, string.Empty, Test.Framework.TestVerdict
                .Pass, Test.Framework.TestVerdict.Fail);
            factory = DDS.DomainParticipantFactory.GetInstance();
            if (factory == null)
            {
                result.Result = "DomainParticipantFactory could not be initialized.";
                return result;
            }

            if (factory.GetDefaultParticipantQos(out pqosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default DomainParticipantQos could not be resolved.";
                return result;
            }
            participant = factory.CreateParticipant(string.Empty, ref pqosHolder, null, 0);
            if (participant == null)
            {
                result.Result = "Creation of DomainParticipant failed.";
                return result;
            }
            typeSupport = new mod.tstTypeSupport();
            if (typeSupport == null)
            {
                result.Result = "Creation of tstTypeSupport failed.";
                this.Cleanup(factory, participant);
                return result;
            }
            System.Console.WriteLine("1");
            rc = typeSupport.RegisterType(participant, "my_type");
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Register type failed.";
                this.Cleanup(factory, participant);
                return result;
            }

            System.Console.WriteLine("2");
            if (participant.GetDefaultTopicQos(out topQosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default TopicQos could not be resolved.";
                this.Cleanup(factory, participant);
                return result;
            }
            topic = participant.CreateTopic("my_topic", "my_type", ref topQosHolder);//, null,0);
            if (topic == null)
            {
                result.Result = "Topic could not be created.";
                this.Cleanup(factory, participant);
                return result;
            }

            System.Console.WriteLine("3");
            if (participant.GetDefaultSubscriberQos(out sqosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default SubscriberQos could not be resolved.";
                this.Cleanup(factory, participant);
                return result;
            }
            subscriber = participant.CreateSubscriber(ref sqosHolder);//, null, 0);
            if (subscriber == null)
            {
                result.Result = "Subscriber could not be created.";
                this.Cleanup(factory, participant);
                return result;
            }

            System.Console.WriteLine("4");
            if (subscriber.GetDefaultDataReaderQos(out dqosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default DataReaderQos could not be resolved.";
                this.Cleanup(factory, participant);
                return result;
            }
            listener = new test.sacs.MyDataReaderListener();
            datareader = (mod.tstDataReader)subscriber.CreateDataReader(topic, ref dqosHolder, listener,
                DDS.StatusKind.Any ^ DDS.StatusKind.DataOnReaders);
            if (datareader == null)
            {
                result.Result = "DataReader could not be created.";
                this.Cleanup(factory, participant);
                return result;
            }

            System.Console.WriteLine("5");
            if (participant.GetDefaultPublisherQos(out pubQosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default PublisherQos could not be resolved.";
                this.Cleanup(factory, participant);
                return result;
            }
            publisher = participant.CreatePublisher(ref pubQosHolder);//, null, 0);
            if (publisher == null)
            {
                result.Result = "Publisher could not be created.";
                this.Cleanup(factory, participant);
                return result;
            }

            System.Console.WriteLine("6");
            if (publisher.GetDefaultDataWriterQos(out wqosHolder) != DDS.ReturnCode.Ok)
            {
                result.Result = "Default DataWriterQos could not be resolved.";
                this.Cleanup(factory, participant);
                return result;
            }
            datawriter = (mod.tstDataWriter)publisher.CreateDataWriter(topic, ref wqosHolder, null, 0);
            if (datawriter == null)
            {
                result.Result = "DataWriter could not be created.";
                this.Cleanup(factory, participant);
                return result;
            }
            try
            {

                System.Console.WriteLine("7");
                System.Threading.Thread.Sleep(3000);

                System.Console.WriteLine("8");
            }
            catch (System.Exception e)
            {
                System.Console.WriteLine(e);
            }
            if (!listener.onLivelinessChangedCalled)
            {
                result.Result = "on_liveliness_changed does not work properly.";
                this.Cleanup(factory, participant);
                return result;
            }
            listener.Reset();

            System.Console.WriteLine("9");
            mod.tst t = new mod.tst();
            t.long_1 = 1;
            t.long_2 = 2;
            t.long_3 = 3;

            rc = datawriter.Write(t, DDS.InstanceHandle.Nil);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Data could not be written.";
                this.Cleanup(factory, participant);
                return result;
            }
            try
            {

                System.Console.WriteLine("10");
                System.Threading.Thread.Sleep(3000);

                System.Console.WriteLine("111");
            }
            catch (System.Exception e)
            {

                System.Console.WriteLine("123");
                System.Console.WriteLine(e);
            }
            if (!listener.onDataAvailableCalled)
            {

                System.Console.WriteLine("11");
                result.Result = "on_data_available does not work properly.";
                this.Cleanup(factory, participant);
                return result;
            }
            listener.Reset();
            rc = publisher.DeleteDataWriter(datawriter);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "DataWriter could not be deleted.";
                this.Cleanup(factory, participant);
                return result;
            }
            try
            {
                System.Threading.Thread.Sleep(3000);
            }
            catch (System.Exception e)
            {
                System.Console.WriteLine(e);
            }
            if (!listener.onLivelinessChangedCalled)
            {
                result.Result = "on_liveliness_changed does not work properly (2).";
                this.Cleanup(factory, participant);
                return result;
            }

            rc = datareader.SetListener(listener, 0);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Listener could not be attached.";
                this.Cleanup(factory, participant);
                return result;
            }
            rc = datareader.SetListener(null, 0);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Listener could not be detached.";
                this.Cleanup(factory, participant);
                return result;
            }
            rc = subscriber.DeleteDataReader(datareader);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Delete datareader failed.";
                this.Cleanup(factory, participant);
                return result;
            }
            listener.Reset();
            dqosHolder.Durability.Kind = DDS.DurabilityQosPolicyKind.TransientDurabilityQos;
            datareader = (mod.tstDataReader)subscriber.CreateDataReader(topic, ref dqosHolder, listener,
                DDS.StatusKind.Any ^ DDS.StatusKind.DataOnReaders);
            try
            {
                System.Threading.Thread.Sleep(3000);
            }
            catch (System.Exception e)
            {
                System.Console.WriteLine(e);
            }
            listener.Reset();
            datawriter = (mod.tstDataWriter)publisher.CreateDataWriter(topic, ref wqosHolder, null, 0);
            try
            {
                System.Threading.Thread.Sleep(3000);
            }
            catch (System.Exception e)
            {
                System.Console.WriteLine(e);
            }
            if (!listener.onRequestedIncompatibleQosCalled)
            {
                result.Result = "on_requested_incompatible_qos does not work properly.";
                this.Cleanup(factory, participant);
                return result;
            }
            rc = participant.DeleteContainedEntities();
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Delete contained entities failed.";
                this.Cleanup(factory, participant);
                return result;
            }
            rc = factory.DeleteParticipant(participant);
            if (rc != DDS.ReturnCode.Ok)
            {
                result.Result = "Delete DomainParticipant failed.";
                return result;
            }
            result.Result = expResult;
            result.Verdict = Test.Framework.TestVerdict.Pass;
            return result;
        }

        private void Cleanup(DDS.DomainParticipantFactory f, DDS.IDomainParticipant p)
        {
            if (p != null)
            {
                p.DeleteContainedEntities();
            }
            if (f != null)
            {
                f.DeleteParticipant(p);
            }
        }
    }
}
